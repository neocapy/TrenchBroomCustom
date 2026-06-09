"""Edit-batch builder for POST /edit.

Ops are queued on an :class:`EditBatch` and sent in one request. Each
creating op returns a :class:`Ref` usable as a handle in later ops of
the same batch (serialized as ``"@name"`` on the wire). After commit,
each Ref resolves to the real handle(s) the server allocated.
"""

from __future__ import annotations

import math
from typing import TYPE_CHECKING, Any, Iterable, Literal, Sequence, Union

from .errors import EditFailed, TrenchBroomError
from .types import (
    Bounds,
    FaceSel,
    Handle,
    OpResult,
    Vec3,
    VecLike,
    _bounds,
    _vec3,
)

if TYPE_CHECKING:
    from .client import Document

HandleLike = Union[Handle, "Ref"]
FaceRefLike = Union[FaceSel, "tuple[HandleLike, int]"]
BoundsLike = Union[Bounds, "tuple[VecLike, VecLike]"]


class Ref:
    """Pending result of an op within an :class:`EditBatch`.

    Usable as a handle in later ops of the same batch. After the batch
    commits, ``.handle`` / ``.handles`` hold the server-assigned values.
    """

    def __init__(self, batch: EditBatch, name: str, index: int) -> None:
        self._batch = batch
        self._name = name
        self._index = index

    @property
    def wire(self) -> str:
        return f"@{self._name}"

    @property
    def result(self) -> OpResult:
        """The committed OpResult for this op. Raises if not committed."""
        results = self._batch.results
        if results is None:
            raise TrenchBroomError("edit batch has not been committed")
        return results[self._index]

    @property
    def handle(self) -> Handle:
        """The single handle this op produced (brush, entity)."""
        r = self.result
        if r.handle is not None:
            return r.handle
        if r.handles is not None and len(r.handles) == 1:
            return r.handles[0]
        raise TrenchBroomError(f"op {self._index} produced no single handle: {r}")

    @property
    def handles(self) -> tuple[Handle, ...]:
        """All handles this op produced (csg, clip)."""
        r = self.result
        if r.handles is not None:
            return r.handles
        if r.handle is not None:
            return (r.handle,)
        raise TrenchBroomError(f"op {self._index} produced no handles: {r}")

    def __repr__(self) -> str:
        return f"Ref({self.wire})"


def _h(h: HandleLike) -> int | str:
    return h.wire if isinstance(h, Ref) else int(h)


def _hs(hs: Iterable[HandleLike]) -> list[int | str]:
    return [_h(h) for h in hs]


def _face_ref(f: FaceRefLike) -> dict[str, Any]:
    if isinstance(f, FaceSel):
        return {"brush": f.brush, "face": f.face}
    brush, face = f
    return {"brush": _h(brush), "face": int(face)}


class EditBatch:
    """Builder for one POST /edit request (one undo step).

    Use as a context manager: commits on clean exit, discards on
    exception. Atomic by default — the first failing op rolls back the
    whole batch and raises :class:`EditFailed`. With
    ``on_error="continue"``, failed ops are skipped, survivors commit,
    and no exception is raised (inspect ``results``).
    """

    def __init__(self, document: Document, on_error: Literal["fail", "continue"] = "fail") -> None:
        self._document = document
        self._on_error: Literal["fail", "continue"] = on_error
        self._ops: list[dict[str, Any]] = []
        self._refnames: set[str] = set()
        self.results: list[OpResult] | None = None

    # -- plumbing ---------------------------------------------------------

    def _add(self, op: dict[str, Any], as_: str | None = None) -> Ref:
        index = len(self._ops)
        name = as_ or f"r{index}"
        if name in self._refnames:
            raise ValueError(f"duplicate ref name: {name}")
        self._refnames.add(name)
        op["as"] = name
        self._ops.append(op)
        return Ref(self, name, index)

    def commit(self) -> list[OpResult]:
        """Send the batch. Raises EditFailed on atomic rollback."""
        if self.results is not None:
            raise TrenchBroomError("edit batch already committed")
        self.results = self._document._commit_edit(self._ops, self._on_error)
        return self.results

    def __enter__(self) -> EditBatch:
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        if exc_type is None:
            self.commit()

    # -- ops --------------------------------------------------------------

    def select(
        self,
        nodes: Iterable[HandleLike] | None = None,
        faces: Iterable[FaceRefLike] | None = None,
        mode: Literal["replace", "add", "remove"] = "replace",
    ) -> Ref:
        op: dict[str, Any] = {"op": "select", "mode": mode}
        if nodes is not None:
            op["nodes"] = _hs(nodes)
        if faces is not None:
            op["faces"] = [_face_ref(f) for f in faces]
        return self._add(op)

    def _brush(
        self, shape: dict[str, Any], material: str | None, layer: HandleLike | None, as_: str | None
    ) -> Ref:
        op: dict[str, Any] = {"op": "brush", "shape": shape}
        if material is not None:
            op["material"] = material
        if layer is not None:
            op["layer"] = _h(layer)
        return self._add(op, as_)

    def brush_box(
        self,
        bounds: BoundsLike,
        material: str | None = None,
        layer: HandleLike | None = None,
        as_: str | None = None,
    ) -> Ref:
        return self._brush({"box": _bounds(bounds).to_json()}, material, layer, as_)

    def brush_cylinder(
        self,
        bounds: BoundsLike,
        axis: Literal["x", "y", "z"] = "z",
        sides: int = 16,
        material: str | None = None,
        layer: HandleLike | None = None,
        as_: str | None = None,
    ) -> Ref:
        shape = {"cylinder": {"bounds": _bounds(bounds).to_json(), "axis": axis, "sides": sides}}
        return self._brush(shape, material, layer, as_)

    def brush_points(
        self,
        points: Sequence[VecLike],
        material: str | None = None,
        layer: HandleLike | None = None,
        as_: str | None = None,
    ) -> Ref:
        """Convex hull of >= 4 points."""
        shape = {"points": [list(_vec3(p)) for p in points]}
        return self._brush(shape, material, layer, as_)

    def entity(
        self,
        classname: str,
        position: VecLike | None = None,
        properties: dict[str, str] | None = None,
        brushes: Iterable[HandleLike] | None = None,
        as_: str | None = None,
    ) -> Ref:
        op: dict[str, Any] = {"op": "entity", "classname": classname}
        if position is not None:
            op["position"] = list(_vec3(position))
        if properties is not None:
            op["properties"] = properties
        if brushes is not None:
            op["brushes"] = _hs(brushes)
        return self._add(op, as_)

    def set_props(
        self,
        handle: HandleLike,
        set: dict[str, str] | None = None,
        remove: Iterable[str] | None = None,
    ) -> Ref:
        op: dict[str, Any] = {"op": "setProps", "handle": _h(handle)}
        if set is not None:
            op["set"] = set
        if remove is not None:
            op["remove"] = list(remove)
        return self._add(op)

    def paint(
        self,
        faces: Iterable[FaceRefLike],
        material: str | None = None,
        x_offset: float | None = None,
        y_offset: float | None = None,
        rotation: float | None = None,
        x_scale: float | None = None,
        y_scale: float | None = None,
    ) -> Ref:
        attrs: dict[str, Any] = {}
        for key, val in (
            ("material", material),
            ("xOffset", x_offset),
            ("yOffset", y_offset),
            ("rotation", rotation),
            ("xScale", x_scale),
            ("yScale", y_scale),
        ):
            if val is not None:
                attrs[key] = val
        op = {"op": "paint", "faces": [_face_ref(f) for f in faces], "attributes": attrs}
        return self._add(op)

    def _transform(self, transform: dict[str, Any], handles: Iterable[HandleLike] | None) -> Ref:
        op: dict[str, Any] = {"op": "transform", "transform": transform}
        if handles is not None:
            op["handles"] = _hs(handles)
        return self._add(op)

    def translate(self, offset: VecLike, handles: Iterable[HandleLike] | None = None) -> Ref:
        """Translate `handles` (or the current selection if omitted)."""
        return self._transform({"translate": list(_vec3(offset))}, handles)

    def rotate(
        self,
        center: VecLike,
        axis: VecLike,
        angle: float,
        handles: Iterable[HandleLike] | None = None,
        degrees: bool = False,
    ) -> Ref:
        if degrees:
            angle = math.radians(angle)
        t = {"rotate": {"center": list(_vec3(center)), "axis": list(_vec3(axis)), "angle": angle}}
        return self._transform(t, handles)

    def scale(
        self, center: VecLike, factors: VecLike, handles: Iterable[HandleLike] | None = None
    ) -> Ref:
        t = {"scale": {"center": list(_vec3(center)), "factors": list(_vec3(factors))}}
        return self._transform(t, handles)

    def transform_matrix(
        self, matrix: Sequence[float], handles: Iterable[HandleLike] | None = None
    ) -> Ref:
        """Row-major 4x4 matrix, 16 numbers (numpy: ``m.flatten()``)."""
        m = [float(x) for x in matrix]
        if len(m) != 16:
            raise ValueError(f"matrix must have 16 elements, got {len(m)}")
        return self._transform({"matrix": m}, handles)

    def delete(self, handles: Iterable[HandleLike]) -> Ref:
        return self._add({"op": "delete", "handles": _hs(handles)})

    def csg(
        self,
        operation: Literal["subtract", "convexMerge", "intersect", "hollow"],
        handles: Iterable[HandleLike] | None = None,
        as_: str | None = None,
    ) -> Ref:
        op: dict[str, Any] = {"op": "csg", "operation": operation}
        if handles is not None:
            op["handles"] = _hs(handles)
        return self._add(op, as_)

    def csg_subtract(
        self, cutters: Iterable[HandleLike] | None = None, as_: str | None = None
    ) -> Ref:
        """Carve `cutters` out of every brush they touch, then delete them."""
        return self.csg("subtract", cutters, as_)

    def csg_convex_merge(
        self, handles: Iterable[HandleLike] | None = None, as_: str | None = None
    ) -> Ref:
        return self.csg("convexMerge", handles, as_)

    def csg_intersect(
        self, handles: Iterable[HandleLike] | None = None, as_: str | None = None
    ) -> Ref:
        return self.csg("intersect", handles, as_)

    def csg_hollow(self, handles: Iterable[HandleLike] | None = None, as_: str | None = None) -> Ref:
        return self.csg("hollow", handles, as_)

    def clip(
        self,
        point: VecLike | None = None,
        normal: VecLike | None = None,
        points: Sequence[VecLike] | None = None,
        keep: Literal["front", "back", "both"] = "front",
        handles: Iterable[HandleLike] | None = None,
        as_: str | None = None,
    ) -> Ref:
        """Clip brushes by a plane: either point+normal, or 3 points
        (normal = cross(p2-p1, p3-p1)). "front" keeps the side the
        normal points into."""
        if points is not None:
            if point is not None or normal is not None:
                raise ValueError("give either points or point+normal, not both")
            if len(points) != 3:
                raise ValueError("plane needs exactly 3 points")
            plane: dict[str, Any] = {"points": [list(_vec3(p)) for p in points]}
        elif point is not None and normal is not None:
            plane = {"point": list(_vec3(point)), "normal": list(_vec3(normal))}
        else:
            raise ValueError("give either points or point+normal")
        op: dict[str, Any] = {"op": "clip", "plane": plane, "keep": keep}
        if handles is not None:
            op["handles"] = _hs(handles)
        return self._add(op, as_)
