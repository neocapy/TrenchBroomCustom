"""Blocking client for the TrenchBroom HTTP control API."""

from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Iterable, Literal, Sequence

from .edit import EditBatch
from .errors import ApiError, ConnectionFailed, EditFailed, TrenchBroomError
from .types import (
    DocumentInfo,
    EntityClass,
    FaceSel,
    Handle,
    HistoryResult,
    MaterialCollection,
    NodeDetail,
    NodeRef,
    NodeSummary,
    NodeType,
    OpResult,
    RayHit,
    Selection,
    VecLike,
    Bounds,
    _vec3,
)


class TrenchBroomClient:
    """Connection to a running TrenchBroom instance.

    The API is loopback-only; the default base URL matches the editor's
    default port.
    """

    def __init__(self, base_url: str = "http://127.0.0.1:28196", timeout: float = 30.0) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout

    # -- transport ---------------------------------------------------------

    def _request(
        self, method: str, path: str, query: dict[str, Any] | None = None, body: Any = None
    ) -> Any:
        url = self.base_url + path
        if query:
            url += "?" + urllib.parse.urlencode(query)
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(
            url, data=data, method=method, headers={"Content-Type": "application/json"}
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                return json.loads(resp.read())
        except urllib.error.HTTPError as e:
            raw = e.read()
            parsed: Any = None
            try:
                parsed = json.loads(raw)
            except Exception:
                pass
            if isinstance(parsed, dict) and "error" in parsed:
                message = parsed["error"]
            else:
                message = raw.decode(errors="replace") or str(e.reason)
            raise ApiError(e.code, message, parsed) from None
        except urllib.error.URLError as e:
            raise ConnectionFailed(f"cannot reach {self.base_url}: {e.reason}") from None

    def _get(self, path: str, **query: Any) -> Any:
        return self._request("GET", path, query=query)

    def _post(self, path: str, body: Any, query: dict[str, Any] | None = None) -> Any:
        return self._request("POST", path, query=query, body=body)

    # -- documents ----------------------------------------------------------

    def documents(self) -> list[Document]:
        """All open documents, active first."""
        return [Document(self, DocumentInfo.from_json(d)) for d in self._get("/documents")]

    def active_document(self) -> Document:
        """The active document. Raises if no document is open."""
        docs = self.documents()
        if not docs or not docs[0].info.active:
            raise TrenchBroomError("no active document")
        return docs[0]


class Document:
    """One open map document; carries its handle into every call.

    ``info`` is a snapshot from when the object was created — call
    :meth:`refresh` to update it. Node handles are session-scoped: a map
    reload remints them (the document handle survives).
    """

    def __init__(self, client: TrenchBroomClient, info: DocumentInfo) -> None:
        self.client = client
        self.info = info

    @property
    def handle(self) -> Handle:
        return self.info.handle

    def refresh(self) -> DocumentInfo:
        """Re-fetch document info (modified flag, layers, node count...)."""
        for d in self.client._get("/documents"):
            if d["handle"] == self.handle:
                self.info = DocumentInfo.from_json(d)
                return self.info
        raise TrenchBroomError(f"document {self.handle} is no longer open")

    def __repr__(self) -> str:
        return f"Document({self.handle}, {self.info.name!r})"

    # -- reads ---------------------------------------------------------------

    def nodes(self, type: NodeType | None = None) -> list[NodeRef]:
        """All node handles in the document (optionally filtered by type)."""
        query: dict[str, Any] = {"doc": self.handle}
        if type is not None:
            query["type"] = type
        res = self.client._request("GET", "/nodes", query=query)
        return [NodeRef(n["handle"], n["type"]) for n in res["nodes"]]

    def get(self, handles: Iterable[Handle]) -> list[NodeSummary]:
        """Summaries for `handles`. Stale handles are silently dropped;
        use :meth:`validate` if you need to know which."""
        body = {"doc": self.handle, "handles": list(handles), "detail": "summary"}
        res = self.client._post("/nodes/get", body)
        return [NodeSummary.from_json(n) for n in res["nodes"]]

    def get_full(self, handles: Iterable[Handle]) -> list[NodeDetail]:
        """Full detail (entity properties, brush faces, children)."""
        body = {"doc": self.handle, "handles": list(handles), "detail": "full"}
        res = self.client._post("/nodes/get", body)
        return [NodeDetail.from_json(n) for n in res["nodes"]]

    def node(self, handle: Handle) -> NodeDetail:
        """Full detail for one node. Raises if the handle is stale."""
        nodes = self.get_full([handle])
        if not nodes:
            raise TrenchBroomError(f"stale or unknown node handle: {handle}")
        return nodes[0]

    def selection(self) -> Selection:
        res = self.client._get("/selection", doc=self.handle)
        return Selection(
            nodes=tuple(res["nodes"]),
            faces=tuple(FaceSel(f["brush"], f["face"]) for f in res["faces"]),
        )

    def validate(self, handles: Iterable[Handle]) -> tuple[list[Handle], list[Handle]]:
        """Split `handles` into (valid, invalid)."""
        res = self.client._post("/handles/validate", {"doc": self.handle, "handles": list(handles)})
        return res["valid"], res["invalid"]

    def materials(self) -> list[MaterialCollection]:
        res = self.client._get("/materials", doc=self.handle)
        return [
            MaterialCollection(c["name"], tuple(c["materials"])) for c in res["collections"]
        ]

    def entity_classes(self) -> list[EntityClass]:
        res = self.client._get("/entityclasses", doc=self.handle)
        return [
            EntityClass(
                classname=c["classname"],
                type=c["type"],
                description=c.get("description"),
                bounds=Bounds.from_json(c["bounds"]) if "bounds" in c else None,
            )
            for c in res["classes"]
        ]

    def contains(self, points: Sequence[VecLike]) -> list[list[Handle]]:
        """For each point, the handles of nodes containing it."""
        body = {"doc": self.handle, "points": [list(_vec3(p)) for p in points]}
        res = self.client._post("/contains", body)
        return [r["handles"] for r in res["results"]]

    def raycast(
        self,
        origin: VecLike,
        direction: VecLike,
        max_distance: float | None = None,
        ignore: Iterable[Handle] | None = None,
    ) -> list[RayHit]:
        """All hits of one ray, sorted near to far ([] = miss). Entry
        faces only; hidden geometry is never hit."""
        return self.raycast_many([(origin, direction)], max_distance, ignore)[0]

    def raycast_many(
        self,
        rays: Sequence[tuple[VecLike, VecLike]],
        max_distance: float | None = None,
        ignore: Iterable[Handle] | None = None,
    ) -> list[list[RayHit]]:
        """Batch raycast; results[i] corresponds to rays[i] (origin, direction)."""
        ignore_list = list(ignore) if ignore is not None else None
        ray_objs = []
        for origin, direction in rays:
            r: dict[str, Any] = {"origin": list(_vec3(origin)), "direction": list(_vec3(direction))}
            if max_distance is not None:
                r["maxDistance"] = max_distance
            if ignore_list is not None:
                r["ignore"] = ignore_list
            ray_objs.append(r)
        res = self.client._post("/raycast", {"doc": self.handle, "rays": ray_objs})
        return [[RayHit.from_json(h) for h in hits] for hits in res["results"]]

    # -- mutation -------------------------------------------------------------

    def edit(self, on_error: Literal["fail", "continue"] = "fail") -> EditBatch:
        """Start an edit batch (one undo step). Use as a context manager:

            with doc.edit() as e:
                box = e.brush_box(([0,0,0], [64,64,16]))
                e.translate([0, 0, 32], handles=[box])
        """
        return EditBatch(self, on_error)

    def _commit_edit(
        self, ops: list[dict[str, Any]], on_error: Literal["fail", "continue"]
    ) -> list[OpResult]:
        query = {"onError": "continue"} if on_error == "continue" else None
        try:
            raw = self.client._post("/edit", {"doc": self.handle, "ops": ops}, query=query)
        except ApiError as e:
            if e.status == 422 and isinstance(e.body, list):
                # Atomic rollback: body is OpResult[], all handles void.
                results = [OpResult.from_json(r) for r in e.body]
                failed = next((r for r in results if not r.ok and not r.skipped), None)
                msg = failed.error if failed and failed.error else "edit batch failed"
                raise EditFailed(msg, results) from None
            raise  # 400/404/409: batch never ran
        return [OpResult.from_json(r) for r in raw]

    # -- history / io -----------------------------------------------------------

    def undo(self) -> HistoryResult:
        res = self.client._post("/undo", {"doc": self.handle})
        return HistoryResult(res["ok"], res.get("name"))

    def redo(self) -> HistoryResult:
        res = self.client._post("/redo", {"doc": self.handle})
        return HistoryResult(res["ok"], res.get("name"))

    def save(self) -> str:
        """Save to disk; returns the path. Raises ApiError (422) if the
        document has never been saved."""
        res = self.client._post("/save", {"doc": self.handle})
        return res["path"]
