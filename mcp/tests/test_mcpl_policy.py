"""MCPL 0.5 grant semantics (SPEC §5.3, §5.4, §6.4, §6.7): the pure policy layer."""
import pytest

from animunculi_mcp.mcpl import policy
from animunculi_mcp.mcpl.server import FEATURE_SETS


def test_pattern_matching_is_one_segment_exact_depth():
    m = policy.pattern_matches
    assert m("pushEvents", "pushEvents")
    assert m("channels.*", "channels.publish")
    assert not m("channels.*", "channels")
    assert not m("contextHooks.*", "contextHooks.beforeInference.inject.beforeUser")
    assert m("contextHooks.beforeInference.inject.*", "contextHooks.beforeInference.inject.beforeUser")
    assert not m("*", "channels.publish")
    assert m("*", "tools")
    assert not m("channels", "channels.publish")  # a bare parent does not grant its leaves


def test_request_is_a_full_statement_and_absence_is_denial():
    g, _ = policy.grant_from_update(None, {"effectiveCapabilities": ["pushEvents"]}, "request")
    assert g.ready and g.has("pushEvents") and not g.has("tools")
    g2, _ = policy.grant_from_update(g, {}, "request")  # absent allowlist in a Request grants nothing
    assert g2.ready and not g2.has("pushEvents")


def test_notification_only_reduces():
    g, _ = policy.grant_from_update(None, {"effectiveCapabilities": ["pushEvents"], "enabled": ["skyrim.*"]}, "request")
    g2, notes = policy.grant_from_update(g, {"effectiveCapabilities": ["pushEvents", "channels.*"],
                                             "disabled": ["skyrim.events"]}, "notification")
    assert not g2.has("channels.publish")  # widening discarded
    assert notes and "discarded" in notes[0]
    assert policy.derive("skyrim.events", FEATURE_SETS["skyrim.events"], g2).reason == "host_disabled"
    assert g2.ready == g.ready


def test_notification_never_establishes_ready():
    g, _ = policy.grant_from_update(None, {"effectiveCapabilities": ["pushEvents"]}, "notification")
    assert not g.ready and not g.has("pushEvents")


def test_contradiction_is_malformed():
    with pytest.raises(policy.PolicyError):
        policy.grant_from_update(None, {"effectiveCapabilities": ["pushEvents"], "deniedCapabilities": ["pushEvents"]}, "request")
    with pytest.raises(policy.PolicyError):
        policy.grant_from_update(None, {"effectiveCapabilities": "pushEvents"}, "request")


def test_derivation_fail_closed_and_selection():
    full = ["pushEvents", "inferenceLifecycle", "contextHooks.beforeInference.inject.beforeUser",
            "channels.register", "channels.lifecycle", "channels.publish", "channels.incoming"]
    g, _ = policy.grant_from_update(None, {"effectiveCapabilities": full}, "request")
    assert all(policy.derive(n, d, g).active for n, d in FEATURE_SETS.items())
    assert policy.receipt(FEATURE_SETS, g)["mode"] == "full"

    g, _ = policy.grant_from_update(None, {"effectiveCapabilities": full, "enabled": ["skyrim.chat", "skyrim.chat.*"]}, "request")
    assert policy.derive("skyrim.events", FEATURE_SETS["skyrim.events"], g).reason == "not_selected"
    assert policy.derive("skyrim.chat.overhear", FEATURE_SETS["skyrim.chat.overhear"], g).active

    g, _ = policy.grant_from_update(None, {"effectiveCapabilities": ["tools", "pushEvents"]}, "request")
    r = policy.receipt(FEATURE_SETS, g, {"skyrim.presence": "no status line"})
    assert r["accepted"] is True and r["mode"] == "degraded"
    missing = {u["featureSet"]: u["missingCapabilities"] for u in r["unavailableFeatures"]}
    assert "skyrim.events" not in missing
    assert missing["skyrim.presence"] == ["contextHooks.beforeInference.inject.beforeUser"]
    assert any("no status line" in n for n in r["notes"])

    assert policy.derive("x", {"description": "d", "uses": []}, g).reason == "invalid_uses"
    assert policy.derive("x", {"description": "d", "uses": ["pushEvents", "telepathy"]}, g).reason == "invalid_uses"


def test_declarations_are_valid_05_shapes():
    for name, decl in FEATURE_SETS.items():
        assert decl["description"] and policy.validate_uses(decl["uses"]) is None, name
