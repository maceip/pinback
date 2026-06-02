#!/usr/bin/env python3
"""Extract the current DS4 interface contract from a git revision.

This is intentionally source-derived. README text can lag; this script reads the
same C files that implement the public and semi-public surfaces we wrap.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any


DS4_FILES = {
    "agent": "ds4_agent.c",
    "bench": "ds4_bench.c",
    "cli": "ds4_cli.c",
    "core": "ds4.c",
    "distributed": "ds4_distributed.c",
    "eval": "ds4_eval.c",
    "server": "ds4_server.c",
    "web": "ds4_web.c",
}


class ContractError(RuntimeError):
    pass


def git(root: Path, *args: str) -> str:
    return subprocess.check_output(["git", "-C", str(root), *args], text=True)


def git_show(root: Path, rev: str, path: str) -> str:
    return git(root, "show", f"{rev}:{path}")


def ordered(values: list[str]) -> list[str]:
    seen: set[str] = set()
    out: list[str] = []
    for value in values:
        if value not in seen:
            seen.add(value)
            out.append(value)
    return out


def source_slice(text: str, start_pat: str, end_pat: str | None = None) -> str:
    start = text.find(start_pat)
    if start < 0:
        return ""
    if not end_pat:
        return text[start:]
    end = text.find(end_pat, start + len(start_pat))
    return text[start:] if end < 0 else text[start:end]


def required_source_slice(text: str, start_pat: str, end_pat: str | None = None) -> str:
    out = source_slice(text, start_pat, end_pat)
    if not out:
        raise ContractError(f"DS4 contract extractor lost source anchor: {start_pat}")
    return out


def extract_flags(text: str) -> list[str]:
    flags = re.findall(r'!strcmp\(arg,\s*"([^"]+)"\)', text)
    return ordered(flags)


def extract_getenvs(files: dict[str, str]) -> list[dict[str, str]]:
    out: list[dict[str, str]] = []
    for name, text in files.items():
        for env in ordered(re.findall(r'getenv\("([^"]+)"\)', text)):
            out.append({"name": env, "source": DS4_FILES[name]})
    return out


def extract_server_http(server: str) -> dict[str, Any]:
    model_aliases = ordered(re.findall(r'!strcmp\(id,\s*"([^"]+)"\)', required_source_slice(
        server, "static bool server_model_alias_known", "static")))
    post_paths = ordered(re.findall(
        r'!strcmp\(hr\.method,\s*"POST"\)\s*&&\s*!strcmp\(hr\.path,\s*"([^"]+)"\)',
        server,
    ))
    endpoints: list[dict[str, str]] = [
        {"method": "OPTIONS", "path": "*"},
        {"method": "GET", "path": "/v1/models"},
    ]
    endpoints.extend({"method": "GET", "path": f"/v1/models/{model}"} for model in model_aliases)
    endpoints.extend({"method": "POST", "path": path} for path in post_paths)
    return {
        "source": "ds4_server.c",
        "model_aliases": model_aliases,
        "endpoints": endpoints,
    }


def extract_slash_commands(agent: str) -> list[dict[str, Any]]:
    known = required_source_slice(agent, "static bool agent_slash_command_known", "static uint64_t")
    exact = re.findall(r'!strcmp\(cmd,\s*"(/[^"]+)"\)', known)
    with_args = re.findall(r'agent_slash_command_with_args\(cmd,\s*"(/[^"]+)"\)', known)
    commands = [{"name": value, "takes_args": False} for value in exact]
    commands.extend({"name": value, "takes_args": True} for value in with_args)
    return commands


def extract_agent_tools(agent: str) -> list[dict[str, Any]]:
    dispatch = required_source_slice(agent, "static char *agent_execute_tool_call", "static char *agent_execute_tool_calls")
    names = ordered(re.findall(r'!strcmp\(call->name,\s*"([^"]+)"\)', dispatch))
    function_names = {
        "read": "agent_tool_read",
        "more": "agent_tool_more",
        "write": "agent_tool_write",
        "list": "agent_tool_list",
        "edit": "agent_tool_edit",
        "search": "agent_tool_search",
        "google_search": "agent_tool_google_search",
        "visit_page": "agent_tool_visit_page",
    }
    tools: list[dict[str, Any]] = []
    for name in names:
        args: list[str] = []
        func = function_names.get(name)
        if func:
            body = required_source_slice(agent, f"static char *{func}", "static ")
            args = ordered(re.findall(r'agent_tool_arg_value\(call,\s*"([^"]+)"\)', body))
        elif name == "bash":
            args = ordered(re.findall(r'agent_tool_arg_value\(call,\s*"([^"]+)"\)', dispatch))
            args = [arg for arg in args if arg in {"command", "timeout_sec", "refresh_sec"}]
        elif name in {"bash_status", "bash_stop"}:
            args = ["job", "pid", "refresh_sec"]
        tools.append({"name": name, "args": args})
    return tools


def extract_browser_cdp(web: str) -> list[str]:
    return ordered(re.findall(r'web_cdp_call(?:_optional)?\([^;]*,\s*"([^"]+\.[^"]+)"', web))


def extract_cli_flags(files: dict[str, str]) -> dict[str, list[str]]:
    distributed_flags = extract_flags(files["distributed"])
    return {
        "ds4": extract_flags(files["cli"]),
        "ds4-agent": extract_flags(files["agent"]),
        "ds4-server": extract_flags(files["server"]),
        "ds4-bench": extract_flags(files["bench"]),
        "ds4-eval": extract_flags(files["eval"]),
        "distributed": distributed_flags,
    }


def require_contains(values: set[str], required: set[str], label: str) -> None:
    missing = sorted(required - values)
    if missing:
        raise ContractError(f"DS4 contract missing {label}: {', '.join(missing)}")


def validate_contract(contract: dict[str, Any]) -> None:
    endpoints = {
        f"{item.get('method')} {item.get('path')}"
        for item in contract.get("server_http", {}).get("endpoints", [])
        if isinstance(item, dict)
    }
    require_contains(
        endpoints,
        {
            "GET /v1/models",
            "GET /v1/models/deepseek-v4-flash",
            "POST /v1/messages",
            "POST /v1/chat/completions",
            "POST /v1/responses",
            "POST /v1/completions",
        },
        "server endpoints",
    )

    slash = {item.get("name") for item in contract.get("agent_slash_commands", []) if isinstance(item, dict)}
    require_contains(slash, {"/save", "/list", "/switch", "/history"}, "agent slash commands")

    tools = {item.get("name") for item in contract.get("agent_tools", []) if isinstance(item, dict)}
    require_contains(
        tools,
        {"read", "write", "edit", "search", "bash", "bash_status", "bash_stop", "visit_page"},
        "agent tools",
    )

    cdp = set(contract.get("browser_cdp_methods", []))
    require_contains(cdp, {"Runtime.evaluate", "Page.navigate", "Target.createTarget"}, "browser CDP methods")

    cli_flags = contract.get("cli_flags", {})
    if not isinstance(cli_flags, dict):
        raise ContractError("DS4 contract cli_flags is not an object")
    for group in ("ds4", "ds4-agent", "ds4-server", "ds4-bench", "ds4-eval", "distributed"):
        values = cli_flags.get(group)
        if not isinstance(values, list) or not values:
            raise ContractError(f"DS4 contract missing CLI flag group: {group}")

    envs = {item.get("name") for item in contract.get("env_vars", []) if isinstance(item, dict)}
    require_contains(envs, {"DS4_CHROME", "DS4_METAL_PREFILL_CHUNK", "DS4_DIST_PREFILL_CHUNK"}, "env vars")


def build_contract(ds4_root: Path, rev: str) -> dict[str, Any]:
    resolved = git(ds4_root, "rev-parse", rev).strip()
    files = {name: git_show(ds4_root, rev, path) for name, path in DS4_FILES.items()}
    contract = {
        "schema_version": 1,
        "ds4_revision": resolved,
        "source": "git",
        "server_http": extract_server_http(files["server"]),
        "agent_slash_commands": extract_slash_commands(files["agent"]),
        "agent_tools": extract_agent_tools(files["agent"]),
        "browser_cdp_methods": extract_browser_cdp(files["web"]),
        "cli_flags": extract_cli_flags(files),
        "env_vars": extract_getenvs(files),
        "file_interfaces": [
            {"path": "~/.ds4/kvcache", "purpose": "agent KV sessions"},
            {"path": "~/.ds4_agent_history", "purpose": "ds4-agent line history"},
            {"path": "~/.ds4_history", "purpose": "ds4 CLI line history"},
        ],
        "wrapper_policy": {
            "contract_source": "DS4 C source at ds4_revision",
            "naming": "preserve DS4 wire names and command names at boundaries",
            "error_style": "explicit ok/error payloads; no silent fallback across contract drift",
        },
    }
    validate_contract(contract)
    return contract


def main() -> int:
    parser = argparse.ArgumentParser(description="Extract or check the DS4 interface contract")
    parser.add_argument("--ds4-root", type=Path, default=Path("/Users/mac/ds4"))
    parser.add_argument("--rev", default="HEAD")
    parser.add_argument("--write", type=Path)
    parser.add_argument("--check", type=Path)
    args = parser.parse_args()

    try:
        contract = build_contract(args.ds4_root, args.rev)
    except ContractError as exc:
        sys.stderr.write(f"DS4 contract extraction failed: {exc}\n")
        return 2
    text = json.dumps(contract, indent=2, sort_keys=True) + "\n"

    if args.write:
        args.write.write_text(text)
    if args.check:
        existing = args.check.read_text()
        if existing != text:
            sys.stderr.write(f"DS4 contract drift: {args.check} does not match {args.rev}\n")
            return 1
    if not args.write and not args.check:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
