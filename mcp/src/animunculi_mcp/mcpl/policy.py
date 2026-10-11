"""MCPL 0.5 capability grants and feature-set derivation (SPEC §5.3, §5.4, §6.4, §6.7).

A Python port of the semantics in `@animalabs/mcpl-core` (`capabilities.ts`) and
heartbeat-mcpl's `mcpl05.ts`, kept pure so it can be unit-tested:

* `effectiveCapabilities` is the sole allowlist, and absence is denial.
* `*` matches exactly one path segment, and the segment counts must be equal.
* A Request establishes readiness and is a full policy statement. An absent allowlist in a
  Request grants nothing.
* A Notification can only reduce, by applying `disabled`. Any widening it carries is discarded.
* A path listed in both `effectiveCapabilities` and `deniedCapabilities` is a malformed message
  (fail closed).
* `uses` must be non-empty and drawn from the closed vocabulary. Otherwise the feature set is
  `invalid_uses`.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

CAPABILITY_PATHS: tuple[str, ...] = (
    "pushEvents",
    "tools",
    "modelInfo",
    "inferenceRequest",
    "inferenceRequest.streaming",
    "inferenceLifecycle",
    "contextHooks.beforeInference.observe",
    "contextHooks.beforeInference.inject.system",
    "contextHooks.beforeInference.inject.beforeUser",
    "contextHooks.beforeInference.inject.afterUser",
    "channels.register",
    "channels.lifecycle",
    "channels.publish",
    "channels.incoming",
    "channels.streaming",
    "channels.acknowledge",
    "channels.typing",
)
_PATHS = frozenset(CAPABILITY_PATHS)


def pattern_matches(pattern: str, path: str) -> bool:
    """§5.4: full-path match; `*` is exactly one segment; no subtree wildcard."""
    if pattern == path:
        return True
    pat, seg = pattern.split("."), path.split(".")
    return len(pat) == len(seg) and all(p == "*" or p == s for p, s in zip(pat, seg))


def granted(effective: list[str] | tuple[str, ...], path: str) -> bool:
    return any(isinstance(e, str) and pattern_matches(e, path) for e in effective)


def name_matches(pattern: str, name: str) -> bool:
    """§6.3 feature-set names: exact, `*`, or a `prefix.*` wildcard."""
    if pattern in (name, "*"):
        return True
    return pattern.endswith(".*") and name.startswith(pattern[:-1])


class PolicyError(ValueError):
    """A malformed `featureSets/update` (bad types, or allow/deny contradiction)."""


@dataclass
class Grant:
    effective: list[str] = field(default_factory=list)
    enabled: list[str] | None = None      # None = unconstrained by name
    disabled: list[str] = field(default_factory=list)
    ready: bool = False                   # only a Request (once answered) makes this True

    def has(self, path: str) -> bool:
        return granted(self.effective, path)

    def selected(self, name: str) -> bool:
        if any(name_matches(p, name) for p in self.disabled):
            return False
        return self.enabled is None or any(name_matches(p, name) for p in self.enabled)


def _strings(params: dict, key: str) -> list[str] | None:
    if key not in params or params[key] is None:
        return None
    value = params[key]
    if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
        raise PolicyError(f'"{key}" must be an array of strings')
    return list(value)


def grant_from_update(previous: Grant | None, params: Any, form: str) -> tuple[Grant, list[str]]:
    """Return (new grant, diagnostics). Raises PolicyError for a malformed Request."""
    prev = previous or Grant()
    if params is None:
        params = {}
    if not isinstance(params, dict):
        raise PolicyError("params must be an object")
    effective = _strings(params, "effectiveCapabilities")
    denied = _strings(params, "deniedCapabilities")
    enabled = _strings(params, "enabled")
    disabled = _strings(params, "disabled") or []

    if form == "notification":
        notes = [f"notification carried {k}; discarded (a Notification can only reduce, §6.7)"
                 for k, v in (("effectiveCapabilities", effective), ("deniedCapabilities", denied), ("enabled", enabled))
                 if v is not None]
        return Grant(list(prev.effective), None if prev.enabled is None else list(prev.enabled),
                     sorted(set(prev.disabled) | set(disabled)), prev.ready), notes

    conflicts = sorted(set(effective or []) & set(denied or []))
    if conflicts:
        raise PolicyError(f"path(s) in both effectiveCapabilities and deniedCapabilities: {', '.join(conflicts)}")
    return Grant(list(effective or []), enabled, disabled, True), []


@dataclass
class FeatureState:
    active: bool
    reason: str | None = None            # invalid_uses | capability_denied | host_disabled | not_selected
    missing: list[str] = field(default_factory=list)


def validate_uses(uses: Any) -> list[str] | None:
    """None when valid, else the unrecognised values (possibly empty for absent/empty uses)."""
    if not isinstance(uses, list) or not uses:
        return []
    bad = [str(u) for u in uses if u not in _PATHS]
    return bad or None


def derive(name: str, decl: dict, grant: Grant) -> FeatureState:
    bad = validate_uses(decl.get("uses"))
    if bad is not None:
        return FeatureState(False, "invalid_uses", bad)
    missing = [u for u in decl["uses"] if not grant.has(u)]
    if missing:
        return FeatureState(False, "capability_denied", missing)
    if any(name_matches(p, name) for p in grant.disabled):
        return FeatureState(False, "host_disabled")
    if grant.enabled is not None and not any(name_matches(p, name) for p in grant.enabled):
        return FeatureState(False, "not_selected")
    return FeatureState(True)


def receipt(decls: dict[str, dict], grant: Grant, consequences: dict[str, str] | None = None) -> dict:
    """§6.7 degradation receipt: testimony about what the server will do, never a request."""
    unavailable, notes = [], []
    for name, decl in decls.items():
        st = derive(name, decl, grant)
        if st.active:
            continue
        unavailable.append({"featureSet": name, "missingCapabilities": st.missing, "effect": "disabled"})
        why = {"invalid_uses": "declaration is invalid", "host_disabled": "disabled by host policy",
               "not_selected": "not listed in enabled",
               "capability_denied": f"needs {', '.join(st.missing)}"}.get(st.reason or "", st.reason or "")
        extra = (consequences or {}).get(name)
        notes.append(f"{name}: {why}" + (f"; {extra}" if extra else ""))
    return {"accepted": True, "mode": "degraded" if unavailable else "full",
            "unavailableFeatures": unavailable, "notes": notes}
